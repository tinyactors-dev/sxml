<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:tree-sitter="urn:sxml:tree-sitter"
    exclude-result-prefixes="tree-sitter">
  <xsl:param name="file" required="yes"/>
  <xsl:param name="language" select="'javascript'"/>
  <xsl:output method="html" html-version="5"/>

  <xsl:template name="xsl:initial-template">
    <pre class="code">
      <xsl:apply-templates select="tree-sitter:parse(unparsed-text($file), $language)"/>
    </pre>
  </xsl:template>

  <!-- Every node a highlights query captured becomes a span. -->
  <xsl:template match="*[@highlight]">
    <span class="{translate(@highlight, '.', ' ')}">
      <xsl:apply-templates/>
    </span>
  </xsl:template>
</xsl:stylesheet>
