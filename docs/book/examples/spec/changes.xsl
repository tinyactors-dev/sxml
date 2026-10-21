<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:spec="urn:example:spec"
    exclude-result-prefixes="spec">
  <xsl:import href="common.xsl"/>
  <xsl:use-package name="spec.xsl" package-version="1.0"/>
  <xsl:output method="html" indent="yes"/>

  <!-- The earlier draft, compared by requirement anchor. -->
  <xsl:param name="old" select="'latch-00.xml'"/>
  <xsl:variable name="old-draft" select="doc($old)"/>

  <xsl:key name="requirement" match="t[@anchor]" use="@anchor"/>

  <xsl:template match="/">
    <html>
      <head>
        <title>Changes in {rfc/@docName}</title>
      </head>
      <body>
        <h1>Changes from {$old-draft/rfc/@docName} to {rfc/@docName}</h1>
        <table>
          <xsl:apply-templates select="//t[@anchor]" mode="change"/>
          <xsl:apply-templates mode="removed" select="
              $old-draft//t[@anchor][empty(key('requirement', @anchor, current()))]"/>
        </table>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="t" mode="change">
    <xsl:variable name="before" select="key('requirement', @anchor, $old-draft)"/>
    <xsl:variable name="status" select="
        if (empty($before)) then 'added'
        else if (spec:text($before) = spec:text(.)) then 'unchanged'
        else 'changed'"/>
    <xsl:if test="$status != 'unchanged'">
      <tr class="{$status}">
        <td><a href="latch.html#{@anchor}">{spec:number(.)}</a></td>
        <td>{$status}</td>
        <td>
          <xsl:if test="$status = 'changed'">
            <del>{spec:text($before)}</del>
            <br/>
          </xsl:if>
          <ins>{spec:text(.)}</ins>
        </td>
      </tr>
    </xsl:if>
  </xsl:template>

  <!-- A requirement that only the old draft has. -->
  <xsl:template match="t" mode="removed">
    <tr class="removed">
      <td>{@anchor}</td>
      <td>removed</td>
      <td><del>{spec:text(.)}</del></td>
    </tr>
  </xsl:template>
</xsl:stylesheet>
