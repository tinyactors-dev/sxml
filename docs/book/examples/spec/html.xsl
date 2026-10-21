<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:spec="urn:example:spec"
    exclude-result-prefixes="spec">
  <!-- The shared rules, with a lower import precedence than these. -->
  <xsl:import href="common.xsl"/>
  <xsl:use-package name="spec.xsl" package-version="1.0"/>
  <xsl:output method="html" indent="yes"/>

  <xsl:template match="/">
    <html>
      <head>
        <title>{rfc/front/title}</title>
      </head>
      <body>
        <h1>{rfc/front/title}</h1>
        <p class="draft">{rfc/@docName}</p>
        <nav>
          <ol class="toc">
            <xsl:apply-templates select="rfc/middle/section" mode="toc"/>
          </ol>
        </nav>
        <xsl:apply-templates select="rfc/middle/section, rfc/back/references"/>
        <section id="requirements">
          <h2>Index of Requirements</h2>
          <table>
            <xsl:apply-templates select="//t[@anchor]" mode="index"/>
          </table>
        </section>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="section">
    <section id="{@anchor}">
      <h2>{spec:number(.)}. {name}</h2>
      <xsl:apply-templates select="* except name"/>
    </section>
  </xsl:template>

  <xsl:template match="t">
    <p><xsl:apply-templates/></p>
  </xsl:template>

  <xsl:template match="t[@anchor]">
    <p id="{@anchor}" class="requirement">
      <span class="number">{spec:number(.)}</span>
      <xsl:text> </xsl:text>
      <xsl:apply-templates/>
    </p>
  </xsl:template>

  <!-- A link around the text that common.xsl makes. -->
  <xsl:template match="xref">
    <a href="#{@target}"><xsl:apply-imports/></a>
  </xsl:template>

  <xsl:template match="bcp14">
    <span class="bcp14 {spec:strength(.)}">{.}</span>
  </xsl:template>

  <xsl:template match="dl | dd">
    <xsl:copy><xsl:apply-templates/></xsl:copy>
  </xsl:template>

  <xsl:template match="dt">
    <dt id="{@anchor}"><xsl:apply-templates/></dt>
  </xsl:template>

  <xsl:template match="sourcecode">
    <pre>{.}</pre>
  </xsl:template>

  <xsl:template match="references">
    <section id="{@anchor}">
      <h2>{name}</h2>
      <xsl:apply-templates select="references"/>
      <xsl:if test="reference">
        <dl>
          <xsl:apply-templates select="reference"/>
        </dl>
      </xsl:if>
    </section>
  </xsl:template>

  <xsl:template match="reference">
    <dt id="{@anchor}">[{@anchor}]</dt>
    <dd>{string-join(front/author/@fullname, ', ')}, "{front/title}",
      {front/date/@month} {front/date/@year}.</dd>
  </xsl:template>

  <xsl:template match="section" mode="toc">
    <li>
      <a href="#{@anchor}">{spec:number(.)}. {name}</a>
      <xsl:if test="section">
        <ol>
          <xsl:apply-templates select="section" mode="toc"/>
        </ol>
      </xsl:if>
    </li>
  </xsl:template>

  <xsl:template match="t" mode="index">
    <tr>
      <td><a href="#{@anchor}">{spec:number(.)}</a></td>
      <td>{bcp14}</td>
      <td>{ancestor::section[1] ! (spec:number(.) || '. ' || name)}</td>
    </tr>
  </xsl:template>
</xsl:stylesheet>
